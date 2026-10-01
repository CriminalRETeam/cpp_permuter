struct Ped { int a, b, c; int state; };
void g(int v);

bool IsReady(Ped* p)
{
    if (p->a <= p->b)
    {
        return true;
    }
    return false;
}
