struct Ped { int a, b, c; int state; };
void g(int v);

bool IsReady(Ped* p)
{
    return p->a <= p->b;
}
