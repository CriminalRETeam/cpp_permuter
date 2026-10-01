struct Ped { int a, b, c; int state; };
void g(int v);

void Update(Ped* p)
{
    if (p->state)
    {
        g(1);
    }
    else
    {
        g(2);
    }
    p->c = 5;
}
