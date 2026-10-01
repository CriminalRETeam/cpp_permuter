struct Ped { int a, b, c; int state; int get_a() { return a; } };
void g(int v);

void Pair(Ped* p)
{
    int x_arr[2];
    x_arr[0] = p->a;
    x_arr[1] = p->b;
    g(x_arr[0]);
    g(x_arr[1]);
}
